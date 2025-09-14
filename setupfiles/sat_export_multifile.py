import sys
import json
from datetime import datetime, timedelta
import math
from geopy.geocoders import Nominatim
from geopy.distance import geodesic
from PyQt5.QtWidgets import (
    QApplication, QWidget, QVBoxLayout, QLabel, QPushButton,
    QLineEdit, QHBoxLayout, QDateEdit, QTimeEdit, QCheckBox, QTextEdit
)
from PyQt5.QtCore import QDateTime
import requests
import os

# Fetch latest Starlink TLE data from Celestrak database
TLE_TXT = "starlink_tle.txt"
TLE_JSON = "starlink_tle.json"

url = "https://celestrak.org/NORAD/elements/gp.php?GROUP=starlink&FORMAT=tle"
response = requests.get(url)

if response.status_code == 200:
    with open(TLE_TXT, "w") as file:
        file.write(response.text)
    print(f"TLE data saved as '{TLE_TXT}'")
else:
    print("Failed to retrieve TLE data. HTTP Status code:", response.status_code)

def chunked(lst, size):
    """Yield successive chunks of length 'size' from list 'lst'."""
    for i in range(0, len(lst), size):
        yield lst[i:i + size]

# Converting TLE .txt to a structured JSON file
def parse_tle_file(file_path):
    """Parse classic 3-line blocks (name + 2 TLE lines) from a text file."""
    with open(file_path, 'r') as file:
        lines = file.readlines()

    satellites = []
    i = 0
    while i + 2 < len(lines):
        name = lines[i].strip()
        line1 = lines[i+1].strip()
        line2 = lines[i+2].strip()

        satellites.append({
            "ID": i // 3 + 1, #creates/assigns a unique ID to each satellite
            "name": name,
            "line-1": line1,
            "line-2": line2
        })
        i += 3

    return satellites

def convert_to_json(file_path, output_file):
    satellites = parse_tle_file(file_path)
    with open(output_file, 'w') as json_file:
        json.dump(satellites, json_file, indent=4)

# Create/refresh the JSON from the downloaded text
convert_to_json(TLE_TXT, TLE_JSON)
print(f"Data has been successfully converted to {TLE_JSON}")

# Load TLE JSON
def load_tle_data(filename):
    with open(filename, "r") as file:
        data = json.load(file)
    satellites = {}
    for entry in data:
        satellites[entry["name"]] = {
            "name": entry["name"],
            "line-1": entry["line-1"],
            "line-2": entry["line-2"]
        }
    return satellites

# Orbital elements 
def parse_tle_lines(tle1, tle2):
    """Extract key orbital elements from TLE lines."""
    year = int(tle1[18:20])
    year += 2000 if year < 57 else 1900
    day_of_year = float(tle1[20:32])
    epoch = datetime(year, 1, 1) + timedelta(days=day_of_year - 1)

    i = math.radians(float(tle2[8:16]))
    raan = math.radians(float(tle2[17:25]))
    e = float("0." + tle2[26:33])
    argp = math.radians(float(tle2[34:42]))
    M0 = math.radians(float(tle2[43:51]))
    n = float(tle2[52:63])

    return {'epoch': epoch, 'i': i, 'raan': raan, 'e': e, 'argp': argp, 'M0': M0, 'n': n}

def solve_kepler(M, e, tol=1e-8):
    E = M if e < 0.8 else math.pi
    while True:
        delta = E - e * math.sin(E) - M
        if abs(delta) < tol:
            break
        E -= delta / (1 - e * math.cos(E))
    return E

def orbital_to_eci(tle, dt):
    mu = 398600.4418  # km^3/s^2
    n_rad = tle['n'] * 2 * math.pi / 86400
    a = (mu / (n_rad**2))**(1/3)

    M = (tle['M0'] + n_rad * dt) % (2 * math.pi)
    E = solve_kepler(M, tle['e'])
    v = 2 * math.atan2(math.sqrt(1 + tle['e']) * math.sin(E/2),
                       math.sqrt(1 - tle['e']) * math.cos(E/2))
    r = a * (1 - tle['e'] * math.cos(E))

    x_orb = r * math.cos(v)
    y_orb = r * math.sin(v)
    z_orb = 0

    i, Ω, ω = tle['i'], tle['raan'], tle['argp']
    x = (math.cos(Ω)*math.cos(ω) - math.sin(Ω)*math.sin(ω)*math.cos(i)) * x_orb + \
        (-math.cos(Ω)*math.sin(ω) - math.sin(Ω)*math.cos(ω)*math.cos(i)) * y_orb
    y = (math.sin(Ω)*math.cos(ω) + math.cos(Ω)*math.sin(ω)*math.cos(i)) * x_orb + \
        (-math.sin(Ω)*math.sin(ω) + math.cos(Ω)*math.cos(ω)*math.cos(i)) * y_orb
    z = (math.sin(ω)*math.sin(i)) * x_orb + (math.cos(ω)*math.sin(i)) * y_orb

    return x, y, z

def eci_to_latlon(x, y, z, time):
    R = math.sqrt(x**2 + y**2 + z**2)
    lat = math.asin(z / R)
    seconds_since_midnight = (time - datetime(time.year, time.month, time.day)).total_seconds()
    gst = 2 * math.pi * (seconds_since_midnight / 86164)
    lon = math.atan2(y, x) - gst
    lon = (lon + math.pi) % (2 * math.pi) - math.pi
    return math.degrees(lat), math.degrees(lon)

def calculate_elevation(user_coord, sat_coord):
    lat1, lon1 = map(math.radians, user_coord)
    lat2, lon2 = map(math.radians, sat_coord)

    delta_lon = lon2 - lon1
    delta_lat = lat2 - lat1

    a = math.sin(delta_lat / 2)**2 + math.cos(lat1) * math.cos(lat2) * math.sin(delta_lon / 2)**2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))

    elevation = 90 - math.degrees(c)
    return elevation

# Interface
class SatelliteFinder(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Nearby Satellites by City")
        self.setGeometry(100, 100, 500, 300)

        self.satellites = load_tle_data(TLE_JSON)

        layout = QVBoxLayout()

        city_layout = QHBoxLayout()
        self.city_input = QLineEdit()
        self.city_input.setPlaceholderText("Enter city name (e.g., Berlin)")
        city_layout.addWidget(QLabel("City:"))
        city_layout.addWidget(self.city_input)
        layout.addLayout(city_layout)

        dist_layout = QHBoxLayout()
        self.distance_input = QLineEdit()
        self.distance_input.setPlaceholderText("Distance in km (e.g., 500)")
        dist_layout.addWidget(QLabel("Distance:"))
        dist_layout.addWidget(self.distance_input)
        layout.addLayout(dist_layout)

        # Start date/time
        start_datetime_layout = QHBoxLayout()
        self.start_date_picker = QDateEdit()
        self.start_time_picker = QTimeEdit()

        current_datetime = QDateTime.currentDateTime()
        self.start_date_picker.setDate(current_datetime.date())
        self.start_time_picker.setTime(current_datetime.time())

        start_datetime_layout.addWidget(QLabel("Start Date:"))
        start_datetime_layout.addWidget(self.start_date_picker)
        start_datetime_layout.addWidget(QLabel("Start Time:"))
        start_datetime_layout.addWidget(self.start_time_picker)
        layout.addLayout(start_datetime_layout)

        # Button to set current time
        self.use_current_time_button = QPushButton("Use Current Time")
        self.use_current_time_button.clicked.connect(self.set_current_time)
        layout.addWidget(self.use_current_time_button)

        # Time window toggle
        self.use_time_window_checkbox = QCheckBox("Use Time Window")
        self.use_time_window_checkbox.setChecked(False)
        self.use_time_window_checkbox.toggled.connect(self.toggle_time_window)
        layout.addWidget(self.use_time_window_checkbox)

        # End date/time
        self.end_date_picker = QDateEdit()
        self.end_time_picker = QTimeEdit()

        end_datetime_layout = QHBoxLayout()
        end_datetime_layout.addWidget(QLabel("End Date:"))
        end_datetime_layout.addWidget(self.end_date_picker)
        end_datetime_layout.addWidget(QLabel("End Time:"))
        end_datetime_layout.addWidget(self.end_time_picker)
        layout.addLayout(end_datetime_layout)

        self.result_text = QTextEdit()
        self.result_text.setReadOnly(True)
        self.result_text.setPlaceholderText("Nearby satellites will be listed here.")
        self.result_text.setMinimumHeight(150)
        layout.addWidget(self.result_text)

        self.find_button = QPushButton("Find Nearby Satellites")
        self.find_button.clicked.connect(self.find_nearby_satellites)
        layout.addWidget(self.find_button)

        self.export_button = QPushButton("Export to JSON (60 per file)")
        self.export_button.clicked.connect(self.export_to_json)
        layout.addWidget(self.export_button)

        self.setLayout(layout)

    def set_current_time(self):
        current_datetime = QDateTime.currentDateTime()
        self.start_date_picker.setDate(current_datetime.date())
        self.start_time_picker.setTime(current_datetime.time())

    def toggle_time_window(self):
        is_checked = self.use_time_window_checkbox.isChecked()
        self.end_date_picker.setEnabled(is_checked)
        self.end_time_picker.setEnabled(is_checked)

    def get_datetime(self, start=True):
        if start:
            date = self.start_date_picker.date().toPyDate()
            time = self.start_time_picker.time().toPyTime()
        else:
            date = self.end_date_picker.date().toPyDate()
            time = self.end_time_picker.time().toPyTime()
        return datetime.combine(date, time)

    def _compute_nearby(self, city, distance_km):
        geolocator = Nominatim(user_agent="sat_finder")
        location = geolocator.geocode(city)
        if not location:
            return None, f"City '{city}' not found."

        user_coord = (location.latitude, location.longitude)
        start_time = self.get_datetime(start=True)
        end_time = self.get_datetime(start=False) if self.use_time_window_checkbox.isChecked() else start_time

        nearby = []
        time_step = timedelta(minutes=10)  # Check every 10 minutes

        t = start_time
        while t <= end_time:
            for sat_id, (sat_name, sat) in enumerate(self.satellites.items(), start=1):
                tle = parse_tle_lines(sat["line-1"], sat["line-2"])
                dt = (t - tle['epoch']).total_seconds()
                x, y, z = orbital_to_eci(tle, dt)
                sat_lat, sat_lon = eci_to_latlon(x, y, z, t)
                d = geodesic(user_coord, (sat_lat, sat_lon)).km
                elev = calculate_elevation(user_coord, (sat_lat, sat_lon))

                if d <= distance_km:
                    visibility = elev > 10
                    nearby.append({
                        "id": sat_id,
                        "name": sat_name,
                        "distance_km": round(d, 2),
                        "elevation_deg": round(elev, 2),
                        "visible": visibility,
                        "latitude": sat_lat,
                        "longitude": sat_lon,
                        "datetime_utc": t.isoformat(),
                        "tle": {
                            "line-1": sat["line-1"],
                            "line-2": sat["line-2"]
                        }
                    })
            t += time_step

        return nearby, None

    def find_nearby_satellites(self):
        city = self.city_input.text().strip()
        try:
            distance_km = float(self.distance_input.text().strip())
        except ValueError:
            self.result_text.setPlainText("Please enter a valid distance.")
            return

        nearby, err = self._compute_nearby(city, distance_km)
        if err:
            self.result_text.setPlainText(err)
            return

        if not nearby:
            self.result_text.setPlainText("No satellites nearby.")
            return

        lines = []
        for entry in nearby:
            status = "Visible" if entry["visible"] else "Not Visible"
            lines.append(f'{entry["name"]} ({entry["distance_km"]:.1f} km) - {status} - {entry["datetime_utc"]}')
        self.result_text.setPlainText("\n".join(lines))

    def export_to_json(self):
        city = self.city_input.text().strip()
        try:
            distance_km = float(self.distance_input.text().strip())
        except ValueError:
            self.result_text.setPlainText("Please enter a valid distance.")
            return

        nearby, err = self._compute_nearby(city, distance_km)
        if err:
            self.result_text.setPlainText(err)
            return

        if not nearby:
            self.result_text.setPlainText("No satellites nearby.")
            return

        # Split into multiple JSON files of up to 60 items each
        MAX_PER_FILE = 60
        base_name = "sat_data"
        files_written = []

        for idx, chunk in enumerate(chunked(nearby, MAX_PER_FILE), start=1):
            filename = f"{base_name}_{idx}.json"
            with open(filename, "w") as f:
                json.dump(chunk, f, indent=4)
            files_written.append(f"{filename} ({len(chunk)} satellites)")

        msg = (
            f"Exported {len(nearby)} records into {len(files_written)} file(s):\n" +
            "\n".join(files_written) +
            f"\n\nSaved in: {os.path.abspath(os.getcwd())}"
        )
        self.result_text.setPlainText(msg)

# Main
if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = SatelliteFinder()
    window.show()
    sys.exit(app.exec_())
